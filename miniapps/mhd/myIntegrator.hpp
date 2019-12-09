#include "mfem.hpp"

using namespace std;
using namespace mfem;

// Integrator for the boundary gradient integral from the Laplacian operator
// this is used in the auxiliary variable where the boundary condition is not needed
class BoundaryGradIntegrator: public BilinearFormIntegrator
{
private:
   Vector shape1, dshape_dn, nor;
   DenseMatrix dshape, dshapedxt, invdfdx;

public:
   virtual void AssembleFaceMatrix(const FiniteElement &el1,
                                   const FiniteElement &el2,
                                   FaceElementTransformations &Trans,
                                   DenseMatrix &elmat);
};

void BoundaryGradIntegrator::AssembleFaceMatrix(
   const FiniteElement &el1, const FiniteElement &el2,
   FaceElementTransformations &Trans, DenseMatrix &elmat)
{
   int i, j, ndof1;
   int dim, order;

   dim = el1.GetDim();
   ndof1 = el1.GetDof();

   // set to this for now, integration includes rational terms
   order = 2*el1.GetOrder() + 1;

   nor.SetSize(dim);
   shape1.SetSize(ndof1);
   dshape_dn.SetSize(ndof1);
   dshape.SetSize(ndof1,dim);
   dshapedxt.SetSize(ndof1,dim);
   invdfdx.SetSize(dim);

   elmat.SetSize(ndof1);
   elmat = 0.0;

   const IntegrationRule *ir = &IntRules.Get(Trans.FaceGeom, order);
   for (int p = 0; p < ir->GetNPoints(); p++)
   {
      const IntegrationPoint &ip = ir->IntPoint(p);
      IntegrationPoint eip1;
      Trans.Loc1.Transform(ip, eip1);
      el1.CalcShape(eip1, shape1);
      //d of shape function, evaluated at eip1
      el1.CalcDShape(eip1, dshape);

      Trans.Elem1->SetIntPoint(&eip1);

      CalcInverse(Trans.Elem1->Jacobian(), invdfdx); //inverse Jacobian
      //invdfdx.Transpose();
      Mult(dshape, invdfdx, dshapedxt); // dshapedxt = grad phi* J^-1

      //get normal vector
      Trans.Face->SetIntPoint(&ip);
      if (dim == 1)
      {
         nor(0) = 2*eip1.x - 1.0;
      }
      else
      {
         CalcOrtho(Trans.Face->Jacobian(), nor);
      }

      /* this is probably the old way
      const DenseMatrix &J = Trans.Face->Jacobian(); //is this J^{-1} or J^{T}?
      else if (dim == 2)
      {
         nor(0) =  J(1,0);
         nor(1) = -J(0,0);
      }
      else if (dim == 3)
      {
         nor(0) = J(1,0)*J(2,1) - J(2,0)*J(1,1);
         nor(1) = J(2,0)*J(0,1) - J(0,0)*J(2,1);
         nor(2) = J(0,0)*J(1,1) - J(1,0)*J(0,1);
      }
      */

      // multiply weight into normal, make answer negative
      // (boundary integral is subtracted)
      nor *= -ip.weight;

      dshapedxt.Mult(nor, dshape_dn);

      for (i = 0; i < ndof1; i++)
         for (j = 0; j < ndof1; j++)
         {
            elmat(i, j) += shape1(i)*dshape_dn(j);
         }
   }
}

// Integrator for (tau * Q.grad func , V.grad v)
// Here we always assume V is the advection speed
class StabConvectionIntegrator : public BilinearFormIntegrator
{
private:
   DenseMatrix dshape, gshape, Jinv, V_ir, Q_ir;
   double dt;
   Coefficient *nuCoef;
   MyCoefficient *V, *Q; 

public:
   StabConvectionIntegrator (double dt_, double visc, MyCoefficient &q) : 
       V(&q), Q(NULL), dt(dt_) 
   { nuCoef = new ConstantCoefficient(visc); }
   StabConvectionIntegrator (double dt_, double visc, MyCoefficient &q, MyCoefficient &v) : 
       V(&v), Q(&q), dt(dt_) 
   { nuCoef = new ConstantCoefficient(visc); }

   virtual void AssembleElementMatrix(const FiniteElement &el,
                                      ElementTransformation &Tr,
                                      DenseMatrix &elmat);

   virtual ~StabConvectionIntegrator()
   { delete nuCoef; }
};

void StabConvectionIntegrator::AssembleElementMatrix(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        DenseMatrix &elmat)
{
    double norm, tau;
    double Unorm, invtau;
    int dim = 2;
    int nd = el.GetDof();
    Vector advGrad(nd), advGrad2(nd), vec1(dim), vec2(dim);

    //here we assume 2d quad
    double eleLength = sqrt( Geometry::Volume[el.GetGeomType()] * Tr.Weight() );   

    elmat.SetSize(nd);
    elmat=0.;
    
    //integration order is el.order + grad.order-1 (-1 due to another derivative taken in V)
    int intorder = 2 * (el.GetOrder() + Tr.OrderGrad(&el)-1);
    const IntegrationRule &ir = IntRules.Get(el.GetGeomType(), intorder);

    V->Eval(V_ir, Tr, ir);
    if(Q!=NULL) Q->Eval(Q_ir, Tr, ir);
  
    for (int i = 0; i < ir.GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir.IntPoint(i);
        
        el.CalcDShape(ip, dshape);
        
        Tr.SetIntPoint(&ip);
        norm = ip.weight * Tr.Weight();
        CalcInverse (Tr.Jacobian(), Jinv);
        
        Mult(dshape, Jinv, gshape); 
        
        //compute tau
        double nu = nuCoef->Eval (Tr, ip);
        V_ir.GetColumnReference(i, vec1);
        Unorm = vec1.Norml2();
        invtau = 2.0/dt + 2.0 * Unorm / eleLength + 4.0 * nu / (eleLength * eleLength);
        tau = 1.0/invtau;
        norm *= tau;
 
        gshape.Mult(vec1, advGrad);
        if (Q==NULL) 
        {
            AddMult_a_VVt(norm, advGrad, elmat);
        }
        else{
            Q_ir.GetColumnReference(i, vec2);
            gshape.Mult(vec2, advGrad2);
            AddMult_a_VWt(norm, advGrad, advGrad2, elmat);
        }
    }
}

// Integrator for (tau * func , V.grad v)
class StabMassIntegrator : public BilinearFormIntegrator
{
private:
   Vector shape;
   DenseMatrix dshape, gshape, Jinv, V_ir;
   double dt;
   Coefficient *nuCoef;
   MyCoefficient *V; 

public:
   StabMassIntegrator (double dt_, double visc, MyCoefficient &q) : 
       V(&q), dt(dt_) 
   { nuCoef = new ConstantCoefficient(visc); }
   virtual void AssembleElementMatrix(const FiniteElement &el,
                                      ElementTransformation &Tr,
                                      DenseMatrix &elmat);

   virtual ~StabMassIntegrator()
   { delete nuCoef; }
};

void StabMassIntegrator::AssembleElementMatrix(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        DenseMatrix &elmat)
{
    double norm, tau;
    double Unorm, invtau;
    int dim = 2;
    int nd = el.GetDof();
    Vector advGrad(nd), vec1(dim);

    //here we assume 2d quad
    double eleLength = sqrt( Geometry::Volume[el.GetGeomType()] * Tr.Weight() );   

    elmat.SetSize(nd);
    elmat=0.;
    
    //this is from ConvectionIntegrator, too high?
    int intorder = el.GetOrder() + Tr.OrderGrad(&el) + Tr.Order();
    const IntegrationRule &ir = IntRules.Get(el.GetGeomType(), intorder);

    V->Eval(V_ir, Tr, ir);
  
    for (int i = 0; i < ir.GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir.IntPoint(i);
        
        el.CalcDShape(ip, dshape);
        el.CalcShape(ip, shape);
        
        Tr.SetIntPoint(&ip);
        norm = ip.weight * Tr.Weight();
        CalcInverse (Tr.Jacobian(), Jinv);
        
        Mult(dshape, Jinv, gshape); 
        
        //compute tau
        double nu = nuCoef->Eval (Tr, ip);
        V_ir.GetColumnReference(i, vec1);
        Unorm = vec1.Norml2();
        invtau = 2.0/dt + 2.0 * Unorm / eleLength + 4.0 * nu / (eleLength * eleLength);
        tau = 1.0/invtau;
        norm *= tau;
 
        gshape.Mult(vec1, advGrad);
        
        AddMult_a_VWt(norm, advGrad, shape, elmat);
    }
}

// Integrator for (tau * rhs , V.grad v)
class StabDomainLFIntegrator : public LinearFormIntegrator
{
private:
   DenseMatrix dshape, gshape, Jinv, V_ir;
   double dt;
   Coefficient *nuCoef, &Q;
   MyCoefficient *V; 

public:
   StabDomainLFIntegrator (double dt_, double visc, MyCoefficient &q, Coefficient &QF) : 
       V(&q), dt(dt_), Q(QF) 
   { nuCoef = new ConstantCoefficient(visc); }

   virtual void AssembleRHSElementVect(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        Vector &elvect);

   virtual ~StabDomainLFIntegrator()
   { delete nuCoef; }
};

void StabDomainLFIntegrator::AssembleRHSElementVect(const FiniteElement &el,
                                        ElementTransformation &Tr,
                                        Vector &elvect)
{
    double norm, tau;
    double Unorm, invtau;
    int dim = 2;
    int nd = el.GetDof();
    Vector advGrad(nd), vec1(dim);

    //here we assume 2d quad
    double eleLength = sqrt( Geometry::Volume[el.GetGeomType()] * Tr.Weight() );   

    elvect.SetSize(nd);
    elvect=0.;
    
    int intorder = 2*el.GetOrder() + Tr.OrderGrad(&el)-1;
    const IntegrationRule &ir = IntRules.Get(el.GetGeomType(), intorder);

    V->Eval(V_ir, Tr, ir);
  
    for (int i = 0; i < ir.GetNPoints(); i++)
    {
        const IntegrationPoint &ip = ir.IntPoint(i);
        
        el.CalcDShape(ip, dshape);
        
        Tr.SetIntPoint(&ip);
        norm = ip.weight * Tr.Weight();
        CalcInverse (Tr.Jacobian(), Jinv);
        
        Mult(dshape, Jinv, gshape); 
        
        //compute tau
        double nu = nuCoef->Eval (Tr, ip);
        V_ir.GetColumnReference(i, vec1);
        Unorm = vec1.Norml2();
        invtau = 2.0/dt + 2.0 * Unorm / eleLength + 4.0 * nu / (eleLength * eleLength);
        tau = 1.0/invtau;

        norm *= (tau * Q.Eval (Tr, ip));
 
        gshape.Mult(vec1, advGrad);

        add(elvect, norm, advGrad, elvect);
    }
}